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

    addOne: (disk) =>
      view = new DiskView(model: disk)
      @$('#disk-list').append(view.render())

    addAll: =>
      Disks.each @addOne

  class Disk extends Backbone.Model

  class DiskList extends Backbone.Collection
    model: Disk
    url: '/api/disks'

  window.Disks = new DiskList

  columns = [
    { name: "dev", label: "Device", cell: "string", editable: false },
    { name: "vendor", label: "Vendor", cell: "string", editable: false },
    { name: "model", label: "Model", cell: "string", editable: false },
    { name: "serial", label: "Serial", cell: "string", editable: false },
  ]
  window.Grid = new Backgrid.Grid { columns: columns, collection: window.Disks }
  $('#content').append(window.Grid.render().$el);

  App = new AppView(el: $('#content'))
