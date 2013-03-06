$(document).ready ->
  class AppView extends Backbone.View

#    events:
#      "keypress #new-todo": "createOnEnter"

#    createOnEnter: (event) ->
#      return if event.keyCode != 13
#      Todos.create content: @input.val()
#      $('#new-todo').val ''

    initialize: ->
      #@input = @$('#new-todo')
      Disks.bind 'add', @addOne
      Disks.bind 'refresh', @addAll

      Disks.fetch()
      $.sparkline_display_visible()

    addOne: (disk) =>
      view = new DiskView(model: disk)
      @$('#disk-list').append(view.render())
      $.sparkline_display_visible()

    addAll: =>
      Disks.each @addOne

  class Disk extends Backbone.Model

  class DiskList extends Backbone.Collection
    model: Disk
    url: '/api/disks'

  window.Disks = new DiskList

  # Update the graph every 10 seconds (10*1000 msecs)
  setInterval ->
    window.Disks.fetch()
    true
  , 10000

  class SparklineCell extends Backgrid.Cell
    className: 'sparkline-cell'

    render: ->
      this.$el.empty()
      value = this.model.get(this.column.get("name"))
      console.log value
      opts =
        type: 'bar'
        chartRangeMin: 0
        colorMap:
          '0:0.5': 'blue'
          '0.5:3': 'pink'
          '3:100': 'red'

      this.$el.sparkline value, opts
      setInterval => 
        $.sparkline_display_visible()
        true
      , 100
      true

      return this
 

  columns = [
    { name: "dev", label: "Device", cell: "string", editable: false },
    { name: "vendor", label: "Vendor", cell: "string", editable: false },
    { name: "model", label: "Model", cell: "string", editable: false },
    { name: "serial", label: "Serial", cell: "string", editable: false },
    { name: "last_top_latency", label: "Graph", cell: SparklineCell, editable: false },
  ]

  window.Grid = new Backgrid.Grid { columns: columns, collection: window.Disks }

  $('#content').append(window.Grid.render().$el);

  App = new AppView(el: $('#content'))
